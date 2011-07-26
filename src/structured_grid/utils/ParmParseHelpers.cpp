#include <BoxLib.H>
#include <ParmParseHelpers.H>
#include <ParallelDescriptor.H>
#include <sstream>

#include "Teuchos_StrUtils.hpp"
#include "Utility.H"

typedef std::list<ParmParse::PP_entry>::iterator list_iterator;
typedef std::list<ParmParse::PP_entry>::const_iterator const_list_iterator;

static std::list<std::string>
split_string_to_list(const std::string& str)
{
  std::vector<std::string> tokens = BoxLib::Tokenize(str," ,");
  std::list<std::string> token_list;
  for (int i=0; i<tokens.size(); ++i)
    {
      token_list.push_back(tokens[i]);
    }
  return token_list;
}

void
bldTable (const Teuchos::ParameterList& params,
	  std::list<ParmParse::PP_entry>& tab)
{
  for (Teuchos::ParameterList::ConstIterator i=params.begin(); i!=params.end(); ++i)
    {
      const std::string& name = params.name(i);
      const Teuchos::ParameterEntry& val = params.getEntry(name);

      if (val.isList() )
        {
          std::list<ParmParse::PP_entry> stab;
          
          bldTable(params.sublist(name), stab);
          
          tab.push_back(ParmParse::PP_entry(name,stab));
          
        }
      else
        {
          const std::string string = toString(val.getAny());
          std::string new_string = Teuchos::StrUtils::varSubstitute(string,"{","");
          new_string = Teuchos::StrUtils::varSubstitute(new_string,"}","");

          std::list<std::string> vals = split_string_to_list(new_string);
          tab.push_back(ParmParse::PP_entry(name,vals));
        }
    }
}

static void
print_table (const std::string& pfx, const ParmParse::Table& table)
{

  for ( const_list_iterator li = table.begin(); li != table.end(); ++li )
    {
      if ( li->m_table )
	{
          std::cout << "Record " << li->m_name << std::endl;
          std::string prefix;
          if (pfx!=std::string())
            prefix = pfx + "::";
          prefix += li->m_name;
          print_table(prefix, *li->m_table);

	}
      else
	{
          std::string prefix;
          if (pfx!=std::string())
            prefix = pfx + "::";
          std::cout << prefix << li->m_name << "(nvals = " << li->m_vals.size() << ") " << " :: [";
          int n = li->m_vals.size();
          for ( int i = 0; i < n; i++ )
            {
              std::cout << li->m_vals[i];
              if ( i < n-1 ) std::cout << ", ";
            }
          std::cout << "]" << '\n';
          
	}
    }
}

void
BoxLib::Initialize_ParmParse(const Teuchos::ParameterList& params)
{
  std::list<ParmParse::PP_entry> table;
  bldTable(params,table);
  std::string dumStr;
  ParmParse::appendTable(table);
}


