Use ACE-Build-Tools to compile this repo, see ETCS repo readme.

THE SHAPE OF A MODULE. Every provider here has the same files in the same
places, because the Makefile ace generates for it (ace manifest generate)
finds its headers by place and nowhere else: a header outside these places is
compiled, but it is not hashed into the module's manifest, so the attestation
the loader checks says nothing about it.

  <Name>/<Name>.cc             ETCS_MODULE_EXPORT_MAIN and the tag blocks.
                               Nothing else.
  <Name>/<Name>.h              The work and stream functions (DEFINE_WORK_FUNC,
                               DEFINE_STREAM_FUNC_*), and the helpers only they
                               use. No types.
  <Name>/Contract_<Name>.h     The types: platform headers selected per platform
                               (#if blocks, with the contract typedefs), then the
                               platform-independent headers, then ../../ETCS.h
                               and module_hashes.h, in that order.
  <Name>/<Name>/*.h            Platform-independent types, one header each.
  <Name>/OS/*.h                Platform-specific types, when one implementation
                               serves every platform's #if branch -- or
  <Name>/Linux|Win|Web/*.h     one directory per platform, when each has its own.
  <Name>/scripts/              .etcs scripts; scripts/www/ is the module's
                               browser page.
  <Name>/www/                  pages a native ETCS HttpServer serves.
  <Name>/exports.map           from the template beside this file.

Vendored code sits in its own directory under the module (sqlite, mbedtls,
glfw, clay, stb, vulkan-headers), fetched or pinned by the manifest.

A type header's guard is <NAME>_<FILE>_H__ (DATABASEPROVIDER_PERSISTENCE_H__),
and it includes ../../../core_defs.h and ../../../ontology.h itself. The
generated files -- Makefile, module_hashes.h, *.d -- are ignored.
