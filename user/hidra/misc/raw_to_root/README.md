# .raw to .root converter

### Typical usage

```
hidra_raw_to_root.py ~/run836_260616011204.raw ~/836_2.root --vme-crate '2:V792,4:V792,6:V792,8:V792,10:V792,11:V792,12:V862,14:V775N' --xdc-detids 1 --fers-detids 2 --tracker-detids 3 --chunk-size 1000 -v
```

With, `--writer uproot`, PyROOT will not be needed. The script will be a bit faster but it will produce `double[]` branches instead of `std::vector<double>` created by EuDAQ online.

### Sanity cross check with the online

Use `root -b Compare.C("file_online.root", "file_converted.root"). Change the name of the branch to check, in the code.