/*
  Input Converter

  Copyright 2010-201x held jointly by LANS/LANL, LBNL, and PNNL. 
  Amanzi is released under the three-clause BSD License. 
  The terms of use and "as is" disclaimer for this license are 
  provided in the top-level COPYRIGHT file.

  Authors: Konstantin Lipnikov (lipnikov@lanl.gov)
*/

#ifndef AMANZI_INPUT_CONVERTER_HH_
#define AMANZI_INPUT_CONVERTER_HH_

#include "boost/lambda/lambda.hpp"
#include "boost/bind.hpp"
#include "boost/lexical_cast.hpp"

#define  BOOST_FILESYTEM_NO_DEPRECATED
#include "boost/filesystem/operations.hpp"
#include "boost/filesystem/path.hpp"
#include "boost/format.hpp"

// TPLs
#include "xercesc/dom/DOM.hpp"
#include "xercesc/parsers/XercesDOMParser.hpp"

// Amanzi's
#include "VerboseObject.hh"

namespace Amanzi {
namespace AmanziInput {

// Amanzi version
#define AMANZI_SPEC_VERSION_MAJOR 2
#define AMANZI_SPEC_VERSION_MINOR 2
#define AMANZI_SPEC_VERSION_MICRO 1

// constants
const std::string TYPE_TIME = "time";
const std::string TYPE_NUMERICAL = "numerical";
const std::string TYPE_AREA_MASS_FLUX = "area_mass_flux";
const std::string TYPE_NONE = "none";
const std::string TYPE_NOT_CONSTANT = "not_constant";

XERCES_CPP_NAMESPACE_USE

/* 
* A simple wrapper for XMLString class. It collects memory pointers
* and destroys them later. The focus is on simplicity of its using,
* so that release is never called.
*/
class MemoryManager {
 public:
  MemoryManager() {};
  ~MemoryManager() { Destroy_(); }

  XMLCh* transcode(const char* str) {
    XMLCh* xstr = xercesc::XMLString::transcode(str);
    xchar.push_back(xstr);
    return xstr;
  }

  char* transcode(const XMLCh* xstr) {
    char* str = xercesc::XMLString::transcode(xstr);
    pchar.push_back(str);
    return str;
  }

 private:
  void Destroy_() {
    for (std::vector<char*>::iterator it = pchar.begin(); it != pchar.end(); ++it)
      xercesc::XMLString::release(&*it);
    for (std::vector<XMLCh*>::iterator it = xchar.begin(); it != xchar.end(); ++it)
      xercesc::XMLString::release(&*it);
  }

 private:
  std::vector<char*> pchar;
  std::vector<XMLCh*> xchar;
};

//------------------------------------------------------------------------
// XML helper methods for parsing outside of an InputConverter:

// Creates an XML parser with our desired settings. This parser must be deleted
// after the document has been used.
XercesDOMParser* CreateXMLParser();

// Using the given XML parser, parses the document contained in the file with 
// the given name.
xercesc::DOMDocument* OpenXMLInput(XercesDOMParser* parser,
                                   const std::string& xml_input);
//------------------------------------------------------------------------

class InputConverter {
 public:

  // This constructor opens up the file with the given name, sets up a parser,
  // and parses the file.
  explicit InputConverter(const std::string& input_filename);

  // This constructor uses an already-parsed XML document, and does not 
  // manage the parser.
  InputConverter(const std::string& input_filename, xercesc::DOMDocument* input_doc);

  virtual ~InputConverter();

  // parse various nodes
  void ParseVersion_();
  void ParseConstants_();
  void FilterNodes(xercesc::DOMNode* parent, const std::string& filter);

 protected:
  // Useful tools wrapping low-level DOM commands
  // -- generalization of getElementsByTagNames(): returns node
  //    where all tags (list of names in the given string) are unique 
  //    leaves of the tree.
  xercesc::DOMNode* GetUniqueElementByTagsString_(
      const std::string& tags, bool& flag);

  // -- modification of the previous routine where the first tag 
  //    is replaced by a pointer to document's element
  xercesc::DOMNode* GetUniqueElementByTagsString_(
      const xercesc::DOMNode* node1, const std::string& tags, bool& flag);

  // -- extract child with the given attribute
  xercesc::DOMElement* GetUniqueChildByAttribute_(
      xercesc::DOMNode* node, const char* attr_name, const std::string& attr_value,
      bool& flag, bool exception = false);

  // -- extract and verify children
  // -- extract existing attribute value
  int GetAttributeValueL_(
      xercesc::DOMElement* elem, const char* attr_name,
      const std::string& type = TYPE_NUMERICAL, bool exception = true, int val = 0);
  double GetAttributeValueD_(
      xercesc::DOMElement* elem, const char* attr_name,
      const std::string& type = TYPE_NUMERICAL, bool exception = true, double val = 0.0);
  std::string GetAttributeValueS_(
      xercesc::DOMElement* elem, const char* attr_name,
      const std::string& type = TYPE_NUMERICAL, bool exception = true, std::string val = "");
  std::vector<double> GetAttributeVector_(
      xercesc::DOMElement* elem, const char* attr_name);
 
  // -- extract existing attribute value and verify it
  std::string GetAttributeValueS_(
      xercesc::DOMElement* elem, const char* attr_name, const char* options);

  //    the name of identical nodes will be extracted too
  std::vector<xercesc::DOMNode*> GetSameChildNodes_(
      xercesc::DOMNode* node, std::string& name, bool& flag, bool exception = false);

  // -- extract text content and verify it
  std::string GetTextContentS_(
      xercesc::DOMNode* node, const char* options, bool exception = true);

  // data streaming/trimming/converting
  // -- times
  double TimeStringToValue_(const std::string& time_value);
  double TimeCharToValue_(const char* time_value);
  std::string GetConstantType_(const std::string& val, std::string& parsed_val);

  // -- coordinates
  std::vector<double> MakeCoordinates_(const std::string& array);

  // -- string modifications
  std::vector<std::string> CharToStrings_(const char* namelist);
  std::string TrimString_(char* tmp);

  // -- vector parsing
  int GetPosition_(const std::vector<std::string>& names, const std::string& name);

  // is spec structurally sound?
  // -- mandatory objects
  int IsEmpty(xercesc::DOMNodeList* node_list, const std::string& name, bool exception = true);
   
  // -- error messages
  void ThrowErrorIllformed_(
      const std::string& section, const std::string& type, const std::string& ill_formed);
  void ThrowErrorIllformed_(
      const std::string& section, const std::string& type, const std::string& ill_formed, const std::string& options);
  void ThrowErrorMissattr_(
      const std::string& section, const std::string& type, const std::string& missing, const std::string& name);

 protected:
  // various constants defined by the users
  // consistency check is performed for all but constants_
  std::map<std::string, std::string> constants_time_;  
  std::map<std::string, std::string> constants_numerical_; 
  std::map<std::string, std::string> constants_area_mass_flux_; 
  std::map<std::string, std::string> constants_;  // no check

  std::string xmlfilename_;
  xercesc::DOMDocument* doc_;
  XercesDOMParser* parser_;
};

}  // namespace AmanziInput
}  // namespace Amanzi

#endif