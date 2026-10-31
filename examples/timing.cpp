#include <gd/gd.h>

#include <filesystem>
#include <vector>
#include <stdexcept>
#include <iostream>
#include <ios>
#include <string>

#include <openssl/sha.h>
#include "generator.h"

using namespace gd;
using namespace gd::shorthand; // add >>, || chaining 
using namespace std;

inline void assertError(char const * msg, const string& err)
{
  throw runtime_error(""s + msg + "\n   " +  err );
}

// Report the number of files/new revisions the DB can introduce in multiple (13) directories
// using different commit sizes 
//
// The total commit time is split into the `add` phase (blob object writes) and the
// `commit` phase (tree construction from the collected updates). This attribution is
// what matters when reasoning about object-write performance: the add phase dominates
// for large commits, and it is the phase a batched/packed write path would accelerate.
//
// Output
//                                       Naive(1) - Per file write       Collect(deb) per dir write   Collect(rel) per dir write
//                                      -----------------------------   ---------------------------   ---------------------------
// Commiting :      1 Files / 13 dirs :: 0.00869548s 115.002 files/s :: 0.00413757s 241.688 files/s :: 0.00499475s 200.21 files/s
// Commiting :     10 Files / 13 dirs :: 0.0909334s  109.971 files/s :: 0.0329744s  303.265 files/s :: 0.0227195s  440.15 files/s
// Commiting :    100 Files / 13 dirs :: 1.11s        90.144 files/s :: 0.203213s   492.094 files/s :: 0.146281s   683.616 files/s
// Commiting :  1,000 Files / 13 dirs :: 37.94s       26.358 files/s :: 1.92068s    520.649 files/s :: 1.30155s    768.315 files/s
// Commiting : 10,000 Files / 13 dirs :: 3,259.63s     3.679 files/s :: 15.6622s    638.479 files/s :: 10.3923s    962.248 files/s
//
// (1) Naively add one file at a time, updating the entire tree up to root
// (2) Collect all elements, on commit, update each directory once
//     Collection is a std::map Path -> Vector<Updates> 
//     std::map collects effected directorys grown is O(logN)
//     std::vector capacity is dynmaically growing, no hint provided, growth is linear O(N)
//     Improvement can be moving to std::unordered_map (Constant time) + sorting O(logN)
void speedTest(const std::string& repoPath)
{
  std::cout << "\n\nWrite speed test (repo: " << repoPath << ")" << endl;
  constexpr size_t maxFileSize(1000);

  std::filesystem::remove_all(repoPath);

  //  Add 12 directories x numFiles files in one commit
  //  Each domain resides in its own directory.
  //  Naieve implementation slows down exponantionally with growth of number of files !!!
  const std::vector<std::string> domains{ "AB", "AS", "UT", "AC", "RT", "TZ", "AD", "AZ", "PT", "RS", "PT", "TV", "VZ"};

  auto dbx = selectRepository(repoPath);
  for (size_t numFiles = 1; numFiles <= 10'000; numFiles *= 10)
  {
    auto addStart = chrono::steady_clock::now();

    for (const auto& domain :  domains)
      for (const auto& [id, content] : hsh::elements(numFiles,  maxFileSize))
        dbx >> add( domain + "/" + id, content);

    auto addEnd = chrono::steady_clock::now();

    dbx >> commit("speed", "speedo@here.com", "timing commit\n")
        || [](const auto& err) -> Result<gd::Context> { assertError("Commit failed", err); return gd_unexpected(err); };

    auto commitEnd = chrono::steady_clock::now();

    auto addS    = chrono::duration<double>(addEnd - addStart).count();
    auto commitS = chrono::duration<double>(commitEnd - addEnd).count();
    auto durationS = addS + commitS;

    std::cout << "Commiting : " << right << setw(5) << numFiles  << " Files for " <<  domains.size() << " domains :: "
      << setw(12) <<  durationS << "s   "
      << setw(10) << (numFiles / durationS) << " files/s"
      << "   [ add " << setw(10) << addS << "s | commit " << setw(9) << commitS << "s ]" << endl;

    dbx >> selectBranch("main"); // Reset the tip to HEAD
  }
}

int main(int argc, char** argv) {

  std::string repoPath = "/tmp/test/speedTest";
  bool buffered = false;
  int level = 1;

  // Usage: speed [--buffered] [--level N] [repo-path]
  //   --buffered  enable the buffered (pack-on-commit) write path
  //   --level N   zlib level for the buffered flush (0 = uncompressed, default 1)
  //   repo-path   optional location, to compare filesystems (e.g. btrfs vs tmpfs)
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--buffered")
      buffered = true;
    else if (arg == "--level" && i + 1 < argc)
      level = std::stoi(argv[++i]);
    else
      repoPath = arg;
  }

  if (buffered)
    gd::setBufferedCommits(true, level);

  speedTest(repoPath);

  return 0;
}
