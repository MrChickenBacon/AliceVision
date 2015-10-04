#include <openMVG/sfm/sfm_data_io.hpp>
#include <openMVG/voctree/database.hpp>
#include <openMVG/voctree/databaseIO.hpp>
#include <openMVG/voctree/vocabulary_tree.hpp>
#include <openMVG/voctree/descriptor_loader.hpp>

#include <boost/program_options.hpp> 
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/tail.hpp>

#include <Eigen/Core>

#include <iostream>
#include <fstream>
#include <ostream>
#include <string>
#include <iomanip>

#define POPART_COUT(x) std::cout << x << std::endl
#define POPART_CERR(x) std::cerr << x << std::endl

static const int DIMENSION = 128;

using namespace std;
using namespace boost::accumulators;
namespace po = boost::program_options;
namespace boostfs = boost::filesystem;

typedef openMVG::features::Descriptor<float, DIMENSION> DescriptorFloat;

typedef std::map<size_t, openMVG::voctree::Document> DocumentMap;

std::ostream& operator<<(std::ostream& os, const openMVG::voctree::Matches &matches)
{
  os << "[ ";
  for(const auto &e : matches)
  {
    os << e.id << ", " << e.score << "; ";
  }
  os << "];\n";
  return os;
}

std::ostream& operator<<(std::ostream& os, const openMVG::voctree::Document &doc)
{
  os << "[ ";
  for(const openMVG::voctree::Word &w : doc)
  {
    os << w << ", ";
  }
  os << "];\n";
  return os;
}

std::string myToString(std::size_t i, std::size_t zeroPadding)
{
  stringstream ss;
  ss << std::setw(zeroPadding) << std::setfill('0') << i;
  return ss.str();
}

bool saveDocumentMap(const std::string &filename, const DocumentMap &docs)
{
  std::ofstream fileout(filename);
  if(!fileout.is_open())
    return false;

  for(const auto &d : docs)
  {
    fileout << "d{" << d.first << "} = " << d.second << "\n";
  }

  fileout.close();
  return true;
}

static const std::string programDescription =
        "This program is used to create a database with a provided dataset of image descriptors using a trained vocabulary tree.\n "
        "The database is then queried optionally with another set of images in order to retrieve for each image the set of most similar images in the dataset\n"
        "If another set of images is not provided, the program will perform a sanity check of the database by querying the database using the same images used to build it\n"
        "It takes as input either a list.txt file containing the a simple list of images (bundler format and older OpenMVG version format)\n"
        "or a sfm_data file (JSON) containing the list of images. In both cases it is assumed that the .desc to load are in the same directory as the input file\n"
        "For the vocabulary tree, it takes as input the input.tree (and the input.weight) file generated by createVoctree\n"
        "As a further output option (--outdir), it is possible to specify a directory in which it will create, for each query image (be it a query image of querylist or an image of keylist)\n"
        "it creates a directory with the same name of the image, inside which it creates a list of symbolic links to all the similar images found. The symbolic link naming convention\n"
        "is matchNumber.filename, where matchNumber is the relevant position of the image in the list of matches ([0-r]) and filename is its image file (eg image.jpg)\n";
/*
 * This program is used to create a database with a provided dataset of image descriptors using a trained vocabulary tree
 * The database is then queried with the same images in order to retrieve for each image the set of most similar images in the dataset
 */
int main(int argc, char** argv)
{
  int verbosity = 1; ///< verbosity level
  string weightsName; ///< the filename for the voctree weights
  bool withWeights = false; ///< flag for the optional weights file
  string treeName; ///< the filename of the voctree
  string keylist; ///< the file containing the list of features to use to build the database
  string queryList = ""; ///< the file containing the list of features to use as query
  string outfile; ///< the file in which to save the results
  string outDir; ///< the directory in which save the symlinks of the similar images
  string documentMapFile; ///< the file where to save the document map in matlab format
  bool withOutput = false; ///< flag for the optional output file
  bool withOutDir = false; ///< flag for the optional output directory to save the symlink of the similar images
  bool withQuery = false; ///< it produces an output readable by matlab
  bool matlabOutput = false; ///< it produces an output readable by matlab
  size_t numImageQuery; ///< the number of matches to retrieve for each image

  openMVG::sfm::SfM_Data sfmdata;
  openMVG::sfm::SfM_Data *sfmdataQuery;

  po::options_description desc(programDescription);
  desc.add_options()
          ("help,h", "Print this message")
          ("verbose,v", po::value<int>(&verbosity)->default_value(1), "Verbosity level, 0 to mute")
          ("weights,w", po::value<string>(&weightsName), "Input name for the weight file, if not provided the weights will be computed on the database built with the provided set")
          ("tree,t", po::value<string>(&treeName)->required(), "Input name for the tree file")
          ("keylist,l", po::value<string>(&keylist)->required(), "Path to the list file generated by OpenMVG containing the features to use for building the database")
          ("querylist,q", po::value<string>(&queryList), "Path to the list file to be used for querying the database")
          ("saveDocumentMap", po::value<string>(&documentMapFile), "A matlab file .m where to save the document map of the created database.")
          ("outdir,", po::value<string>(&outDir), "Path to the directory in which save the symlinks of the similar images (it will be create if it does not exist)")
          (",r", po::value<size_t>(&numImageQuery)->default_value(10), "The number of matches to retrieve for each image, 0 to retrieve all the images")
          ("matlab,", po::bool_switch(&matlabOutput)->default_value(matlabOutput), "It produces an output readable by matlab")
          ("outfile,o", po::value<string>(&outfile), "Name of the output file");


  po::variables_map vm;

  try
  {
    po::store(po::parse_command_line(argc, argv, desc), vm);

    if(vm.count("help") || (argc == 1))
    {
      std::cout << desc << std::endl;
      return EXIT_SUCCESS;
    }

    po::notify(vm);
  }
  catch(boost::program_options::required_option& e)
  {
    std::cerr << "ERROR: " << e.what() << std::endl << std::endl;
    std::cout << "Usage:\n\n" << desc << std::endl;
    return EXIT_FAILURE;
  }
  catch(boost::program_options::error& e)
  {
    std::cerr << "ERROR: " << e.what() << std::endl << std::endl;
    std::cout << "Usage:\n\n" << desc << std::endl;
    return EXIT_FAILURE;
  }

  if(vm.count("weights"))
  {
    withWeights = true;
  }
  if(vm.count("outfile"))
  {
    withOutput = true;
  }
  if(vm.count("querylist"))
  {
    withQuery = true;
  }
  if(vm.count("outdir"))
  {
    // check that both query list or klist are a json file
    withOutDir = boostfs::path(keylist).extension().string() == ".json";
    if(withOutDir and withQuery)
    {
      withOutDir = withOutDir and ( boostfs::path(queryList).extension().string() == ".json");
    }
  }


  //************************************************
  // Load vocabulary tree
  //************************************************

  printf("Loading vocabulary tree\n");
  openMVG::voctree::VocabularyTree<DescriptorFloat> tree(treeName);
  cout << "tree loaded with" << endl << "\t" << tree.levels() << " levels" << endl << "\t" << tree.splits() << " branching factor" << endl;


  //************************************************
  // Create the database
  //************************************************

  POPART_COUT("Creating the database...");
  // Add each object (document) to the database
  openMVG::voctree::Database db(tree.words());

  if(withWeights)
  {
    POPART_COUT("Loading weights...");
    db.loadWeights(weightsName);
  }
  else
  {
    POPART_COUT("No weights specified, skipping...");
  }


  //*********************************************************
  // Read the descriptors and populate the database
  //*********************************************************

  std::vector<size_t> featRead;
  POPART_COUT("Reading descriptors from " << keylist);
  DocumentMap documents;

  auto detect_start = std::chrono::steady_clock::now();
  size_t numTotFeatures = openMVG::voctree::populateDatabase(keylist, tree, db, documents, featRead);
  auto detect_end = std::chrono::steady_clock::now();
  auto detect_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(detect_end - detect_start);

  if(numTotFeatures == 0)
  {
    POPART_CERR("No descriptors loaded!!");
    return EXIT_FAILURE;
  }

  POPART_COUT("Done! " << documents.size() << " sets of descriptors read for a total of " << numTotFeatures << " features");
  POPART_COUT("Reading took " << detect_elapsed.count() << " sec");
  
  if(vm.count("saveDocumentMap"))
  {
    saveDocumentMap(documentMapFile, documents);
  }

  if(!withWeights)
  {
    // Compute and save the word weights
    POPART_COUT("Computing weights...");
    db.computeTfIdfWeights();
  }


  //************************************************
  // Query documents or sanity check
  //************************************************

  std::vector<openMVG::voctree::Matches> allMatches;
  size_t wrong = 0;
  if(numImageQuery == 0)
  {
    // if 0 retrieve the score for all the documents of the database
    numImageQuery = db.size();
  }
  std::ofstream fileout;
  if(withOutput)
  {
    fileout.open(outfile, ofstream::out);
  }

  // if the query list is not provided
  if(!withQuery)
  {
    // do a sanity check
    POPART_COUT("Sanity check: querying the database with the same documents");
    db.sanityCheck(numImageQuery, allMatches);
  }
  else
  {
    // otherwise qury the database with the provided query list
    POPART_COUT("Querying the database with the documents in " << queryList);
    openMVG::voctree::queryDatabase(queryList, tree, db, numImageQuery, allMatches);
  }

  if(withOutDir)
  {
    // load the json for the dataset used to build the database
    if(openMVG::sfm::Load(sfmdata, keylist, openMVG::sfm::ESfM_Data::VIEWS))
    {
      POPART_COUT("SfM data loaded from " << keylist << " containing: ");
      POPART_COUT("\tnumber of views      : " << sfmdata.GetViews().size());
    }
    else
    {
      POPART_CERR("Could not load the sfm_data file " << keylist << "!");
      return EXIT_FAILURE;
    }
    // load the json for the dataset used to query the database
    if(withQuery)
    {
      sfmdataQuery = new openMVG::sfm::SfM_Data();
      if(openMVG::sfm::Load(*sfmdataQuery, queryList, openMVG::sfm::ESfM_Data::VIEWS))
      {
        POPART_COUT("SfM data loaded from " << queryList << " containing: ");
        POPART_COUT("\tnumber of views      : " << sfmdataQuery->GetViews().size());
      }
      else
      {
        POPART_CERR("Could not load the sfm_data file " << queryList << "!");
        return EXIT_FAILURE;
      }
    }
    else
    {
      // otherwise sfmdataQuery is just a link to the dataset sfmdata
      sfmdataQuery = &sfmdata;
    }

    // create recursively the provided out dir
    if(!boostfs::exists(boostfs::path(outDir)))
    {
      //			POPART_COUT("creating directory" << outDir);
      boostfs::create_directories(boostfs::path(outDir));
    }

  }

  for(size_t i = 0; i < allMatches.size(); ++i)
  {
    const auto matches = allMatches[i];
    boostfs::path dirname;
    POPART_COUT("Camera: " << i);
    POPART_COUT("query document " << i << " has " << matches.size() << " matches\tBest " << matches[0].id << " with score " << matches[0].score);
    if(withOutput)
    {
      if(not matlabOutput)
      {
        fileout << "Camera: " << i << std::endl;
      }
      else
      {
        fileout << "m{" << i + 1 << "}=";
        fileout << matches;
      }
    }
    if(withOutDir)
    {
      // create a new directory inside outDir with the same name as the query image
      // the query image can be either from the dataset or from the query list if provided

      // to put a symlink to the query image too
      boostfs::path absoluteFilename; //< the abs path to the image
      boostfs::path sylinkName; //< the name used for the symbolic link

      // get the dirname from the filename
      openMVG::sfm::Views::const_iterator it = sfmdataQuery->GetViews().find(i);
      if(it != sfmdataQuery->GetViews().end())
      {
        sylinkName = boostfs::path(it->second->s_Img_path).filename();
        dirname = boostfs::path(outDir) / sylinkName;
        absoluteFilename = boostfs::path(sfmdataQuery->s_root_path) / sylinkName;
      }
      else
      {
        // this is very wrong
        POPART_CERR("Could not find the image file for the document " << i << "!");
        return EXIT_FAILURE;
      }
      boostfs::create_directories(dirname);
      boostfs::create_symlink(absoluteFilename, dirname / sylinkName);
    }
    // now parse all the returned matches 
    for(size_t j = 0; j < matches.size(); ++j)
    {
      POPART_COUT("\t match " << matches[j].id << " with score " << matches[j].score);
      //			POPART_CERR("" <<  i->first << " " << matches[j].id << " " << matches[j].score);
      if(withOutput and not matlabOutput) fileout << i << " " << matches[j].id << " " << matches[j].score << std::endl;

      if(withOutDir)
      {
        // create a new symbolic link inside the current directory pointing to
        // the relevant matching image
        boostfs::path absoluteFilename; //< the abs path to the image
        boostfs::path sylinkName; //< the name used for the symbolic link

        // get the dirname from the filename
        openMVG::sfm::Views::const_iterator it = sfmdata.GetViews().find(matches[j].id);
        if(it != sfmdata.GetViews().end())
        {
          boostfs::path imgName(it->second->s_Img_path);
          sylinkName = boostfs::path(myToString(j, 4) + "." + imgName.filename().string());
          boostfs::path imgPath(sfmdata.s_root_path);
          absoluteFilename = imgPath / imgName;
        }
        else
        {
          // this is very wrong
          POPART_CERR("Could not find the image file for the document " << matches[j].id << "!");
          return EXIT_FAILURE;
        }
        boostfs::create_symlink(absoluteFilename, dirname / sylinkName);
      }
    }

    if(!withQuery)
    {
      // only for the sanity check, check if the best matching image is the document itself
      if(i != matches[0].id)
      {
        ++wrong;
        POPART_COUT("##### wrong match for document " << i);
      }
    }
  }
  if(!withQuery)
  {
    if(wrong)
      POPART_COUT("there are " << wrong << " wrong matches");
    else
      POPART_COUT("no wrong matches!");
  }

  if(withOutput)
  {
    fileout.close();
  }

  return EXIT_SUCCESS;
}
