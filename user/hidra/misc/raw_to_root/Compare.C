#include <TFile.h>
#include <TTree.h>
#include <TLeaf.h>
#include <iostream>
#include <vector>
#include <cmath>

void Compare(const char* f1name, const char* f2name)
{
  TFile f1(f1name);
  TFile f2(f2name);

  auto t1 = (TTree*)f1.Get("hidra");
  auto t2 = (TTree*)f2.Get("hidra");

  if (!t1 || !t2) {
    std::cerr << "ERROR: could not find TTree 'hidra' in one of the files\n";
    return;
  }

  if (!t1->GetBranch("ADCs") || !t2->GetBranch("ADCs")) {
    std::cerr << "ERROR: branch 'ADCs' not found in one of the trees\n";
    return;
  }

  const Long64_t n1 = t1->GetEntries();
  const Long64_t n2 = t2->GetEntries();

  if (n1 != n2) {
    std::cout << "DIFFERENT: number of entries differs: "
              << n1 << " vs " << n2 << "\n";
    return;
  }

  TLeaf* l1 = t1->GetLeaf("ADCs");
  TLeaf* l2 = t2->GetLeaf("ADCs");

  if (!l1 || !l2) {
    std::cerr << "ERROR: could not get leaf 'ADCs'\n";
    return;
  }

  for (Long64_t i = 0; i < n1; ++i) {
    t1->GetEntry(i);
    t2->GetEntry(i);

    const int len1 = l1->GetLen();
    const int len2 = l2->GetLen();

    if (len1 != len2) {
      std::cout << "DIFFERENT at entry " << i
                << ": ADCs length differs: "
                << len1 << " vs " << len2 << "\n";
      return;
    }

    for (int j = 0; j < len1; ++j) {
      const double v1 = l1->GetValue(j);
      const double v2 = l2->GetValue(j);

      if (v1 != v2) {
        std::cout << "DIFFERENT at entry " << i
                  << ", ADCs[" << j << "]: "
                  << v1 << " vs " << v2 << "\n";
        return;
      }
    }
  }

  std::cout << "IDENTICAL: branch ADCs is identical in both files\n";
}
