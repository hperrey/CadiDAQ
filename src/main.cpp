/**
 * Parse the UTF8 ".ini" file via the Boost property_tree
 */

#include <string>
#include <fstream>
#include <iostream>
#include <iomanip>   // std::hex
#include <stdexcept> // exceptions

#include <boost/property_tree/ini_parser.hpp>
#include <boost/program_options.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/foreach.hpp>

#include <boolTranslator.hpp> // changes 'bool' interpretation in ptrees

#include "caen.hpp"

#include <logging.hpp>
#include <settings.hpp>

#include <helper.hpp>       // CadiDAQ helper functions

namespace po = boost::program_options;
namespace pt = boost::property_tree;

#define MAIN_LOG_DEBUG                                          \
  BOOST_LOG_CHANNEL_SEV(lg, "main", boost::log::trivial::debug)
#define MAIN_LOG_INFO                                           \
  BOOST_LOG_CHANNEL_SEV(lg, "main", boost::log::trivial::info)
#define MAIN_LOG_WARN                                             \
  BOOST_LOG_CHANNEL_SEV(lg, "main", boost::log::trivial::warning)
#define MAIN_LOG_ERROR                                          \
  BOOST_LOG_CHANNEL_SEV(lg, "main", boost::log::trivial::error)
#define MAIN_LOG_FATAL                                          \
  BOOST_LOG_CHANNEL_SEV(lg, "main", boost::log::trivial::fatal)


//
// programming configuration into digitizer
//


enum class comDirection {READING, WRITING};

template <typename T> void programWrapper(caen::Digitizer* instance, void (caen::Digitizer::*write)(T), T (caen::Digitizer::*read)(), T &value, comDirection direction){
  try{
    if (direction == comDirection::WRITING){
      // WRITING
      (instance->*write)(value);
    } else {
      // READING
      value = (instance->*read)();
    }
  }
  catch (caen::Error& e){
    // TODO: more fine-grained error handling, more info on log
    MAIN_LOG_ERROR << "Caught exception when communicating with digitizer " << instance->modelName() << ", serial " << instance->serialNumber() << ":";
    if (direction == comDirection::WRITING)
      MAIN_LOG_ERROR << "\t Calling " << e.where() << " with argument '" << std::to_string(value) << "' caused exception: " << e.what();
    else
      MAIN_LOG_ERROR << "\t Calling " << e.where() << " caused exception: " << e.what();
  }
}

template <typename T> void programWrapper(caen::Digitizer* instance, void (caen::Digitizer::*write)(T), T (caen::Digitizer::*read)(), boost::optional<T> &value, comDirection direction){
  // simple additional check to program only if:
  // - optional value is set
  // - or the value is being read out from the digitizer
  if (value){
    programWrapper(instance, write, read, *value, direction);
  } else if (direction == comDirection::READING){
    // reading into dummy variable
    T k;
    programWrapper(instance, write, read, k, direction);
    value = k;
  }
}

void programMaskWrapper(caen::Digitizer* digitizer, void (caen::Digitizer::*write)(uint32_t), uint32_t (caen::Digitizer::*read)(), std::vector<boost::optional<bool>> &vec, std::string name, comDirection direction){
  uint32_t mask = 0;
  // derive the mask in case we are writing it
  if (direction == comDirection::WRITING)
    mask = vec2Mask(vec, digitizer->groups());
  // verify that channel vector -> group mask conversion is consistent and the same as channel -> channel mask, else warn about misconfiguration
  if (mask != vec2Mask(vec, 1)){
    MAIN_LOG_WARN << "Channel mask cannot be exactly mapped to groups of the device for setting '" << name << "'. Using instead group mask of " << mask;
  }
  programWrapper(digitizer, &caen::Digitizer::setChannelEnableMask, &caen::Digitizer::getChannelEnableMask, mask, direction);
  // if reading: now store the retrieved mask it in the vector
  if (direction == comDirection::READING)
    mask2Vec(mask, vec, digitizer->groups());
}


void programSettings(caen::Digitizer* digitizer, cadidaq::registerSettings* settings, comDirection direction){

  programWrapper(digitizer, &caen::Digitizer::setSWTriggerMode, &caen::Digitizer::getSWTriggerMode, settings->swTriggerMode, direction);

  if (digitizer->groups() == 1){
    // no grouped channels
    programMaskWrapper(digitizer, &caen::Digitizer::setChannelEnableMask, &caen::Digitizer::getChannelEnableMask, settings->chEnable, "chEnable", direction);
  } else {
    // channels are grouped
    programMaskWrapper(digitizer, &caen::Digitizer::setGroupEnableMask, &caen::Digitizer::getGroupEnableMask, settings->chEnable, "chEnable", direction);
  }
}

//
// reading config file
//

void read_ini_file(const char *filename)
{

    /* Open the UTF8 .ini file */
    std::ifstream iniStream(filename);

    /* Parse the .ini file via boost::property_tree::ini_parser */
    pt::iptree iniPTree; // ptree w/ case-insensitive comparisons
    pt::ini_parser::read_ini(iniStream, iniPTree);

    // parse the config file to determine number of digitizers
    int NDigitizer = 0;
    for (auto& section : iniPTree){
      if(boost::iequals(boost::algorithm::to_lower_copy(section.first), std::string("daq")))
        continue;
      if(boost::iequals(boost::algorithm::to_lower_copy(section.first), std::string("general")))
        continue;
      NDigitizer++;
    }
    MAIN_LOG_INFO << "Configuration for " << NDigitizer << " digitizer(s) found in config file.";

    std::vector<cadidaq::connectionSettings*> vecLnkSettings;
    std::vector<cadidaq::registerSettings*> vecRegSettings;
    // get the connection details for each digitizer section
    for (auto& section : iniPTree){
      // ignoring "daq" settings for main application
      if(boost::iequals(boost::algorithm::to_lower_copy(section.first), std::string("daq")))
        continue;
      // ignoring "general" section for common digitizer settings (for now)
      if(boost::iequals(boost::algorithm::to_lower_copy(section.first), std::string("general")))
        continue;
      // parse link settings
      std::string digName = section.first;
      pt::iptree &node = iniPTree.get_child(digName);
      cadidaq::connectionSettings* linksettings = new cadidaq::connectionSettings(digName);
      linksettings->parse(&node);
      linksettings->verify();
      // establish connection
      caen::Digitizer* digitizer = nullptr;
      MAIN_LOG_INFO << "Establishing connection to digitizer '" << digName
                    << "' (linkType=" << *linksettings->linkType
                    << ", linkNum=" << *linksettings->linkNum
                    << ", ConetNode=" << *linksettings->conetNode
                    << ", VMEBaseAddress=" << std::to_string(*linksettings->vmeBaseAddress) << ")";
      try{
        digitizer = caen::Digitizer::open(*linksettings->linkType, *linksettings->linkNum, *linksettings->conetNode, *linksettings->vmeBaseAddress);
      }
      catch (caen::Error& e){
        MAIN_LOG_ERROR << "Caught exception when establishing communication with digitizer " << linksettings->getName() << ": " << e.what();
        if (!digitizer){
          // TODO: more fine-grained error handling, more info on log
          MAIN_LOG_ERROR << "Please check the physical connection and the connection settings. If using USB link, please make sure that the CAEN USB driver kernel module is installed and loaded, especially after kernel updates (or use DKMS as explained in INSTALL.md).";
          // won't be able to handle this much more gracefully than:
          exit(EXIT_FAILURE);
        }
      }
      // status printout
      MAIN_LOG_DEBUG << "Connected to digitzer '" << digName << "'" << std::endl
                     << "\t Model:\t\t"             << digitizer->modelName() << " (numeric model number: " << digitizer->modelNo() << ")" << std::endl
                     << "\t NChannels:\t"         << digitizer->channels() << " (in " << digitizer->groups() << " groups)" << std::endl
                     << "\t ADC bits:\t"          << digitizer->ADCbits() << std::endl
                     << "\t license:\t"           << digitizer->license() << std::endl
                     << "\t Form factor:\t"       << digitizer->formFactor() << std::endl
                     << "\t Family code:\t"       << digitizer->familyCode() << std::endl
                     << "\t Serial number:\t"     << digitizer->serialNumber() << std::endl
                     << "\t ROC FW rel.:\t"       << digitizer->ROCfirmwareRel() << ", AMC FW rel.: " << digitizer->AMCfirmwareRel() << std::endl
                     << "\t PCB rev.:\t"          << digitizer->PCBrevision() << std::endl;

      // register setting parsing
      uint nchannels = digitizer->channels();
      cadidaq::registerSettings* regsettings = new cadidaq::registerSettings(section.first, nchannels);
      regsettings->parse(&node);
      regsettings->verify();
      // write register configuration to digitizer
      programSettings(digitizer, regsettings, comDirection::WRITING);
      /* Loop over all sub sections and keys that remained after parsing */
      for (auto& key : node){
        MAIN_LOG_WARN << "Unknown setting in section " << section.first << " ignored: \t" << key.first << " = " << key.second.get_value<std::string>();
      }
      // now read back register configuration from digitizer
      programSettings(digitizer, regsettings, comDirection::READING);
      vecLnkSettings.push_back(linksettings);
      vecRegSettings.push_back(regsettings);
    }

    // write the config back to another file
    pt::iptree ptwrite; // create a new tree
    BOOST_FOREACH(cadidaq::connectionSettings *settings, vecLnkSettings){
      pt::iptree *node = settings->createPTree();
      ptwrite.put_child(settings->getName(), *node);
    }
    BOOST_FOREACH(cadidaq::registerSettings *settings, vecRegSettings){
      pt::iptree &node = ptwrite.get_child(settings->getName());
      settings->fillPTree(&node);
    }
    pt::ini_parser::write_ini("output.ini", ptwrite);
}


int main(int argc, char **argv)
{
    po::options_description desc("MainOptions");
    desc.add_options()
        ("help,h", "Print help message")
        ("file,f", 
            po::value<std::string>()->default_value("test.ini"),
            "The test .ini file");

    po::variables_map vm;
    try
    {
        po::store(po::parse_command_line(argc, argv, desc), vm);
    }
    catch (po::error &e)
    {
        /* Invalid options */
        std::cerr << "ERROR: " << e.what() << std::endl << std::endl;
        std::cout << "Boost property_tree tester:" << std::endl
            << desc << std::endl;
        return 0;
    }

    if (vm.count("help"))
    {
        /* print usage */
        std::cout << "Boost property_tree tester:" << std::endl
                  << desc << std::endl;
        return 0;
    }

    init_console_logging();

    std::string iniFile = vm["file"].as<std::string>().c_str();
    std::cout << "Read ini file: " << iniFile << std::endl;
    read_ini_file(iniFile.c_str());
    MAIN_LOG_INFO << "Program loop terminated. Have a nice day :)";
    return 0;
}

