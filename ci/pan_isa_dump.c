/*
 * Compile a Vulkan compute SPIR-V shader to Mali machine code with Mesa's
 * panfrost back-end, from the build host.
 *
 * Mesa's own panfrost_compile (src/panfrost/clc/pan_compile.c) only accepts its
 * private "precompiled kernel" flavour: the kernel must take a single preamble
 * struct pointer, and any other signature makes nir_precomp_nr_variants()
 * dereference a null parameter and crash (SIGSEGV at pan_compile.c:349).  We
 * want an ordinary Vulkan compute shader, so this tool drives the backend
 * directly:  spirv_to_nir -> pan_preprocess_nir -> pan_postprocess_nir ->
 * pan_shader_compile.
 *
 * Usage: pan_isa_dump <arch> <in.spv> <out.bin>
 *   arch 9 == Valhall, which is what the Mali-G57 on the camellia board is.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "compiler/glsl_types.h"
#include "compiler/spirv/nir_spirv.h"
#include "nir.h"
#include "panfrost/compiler/pan_compiler.h"
#include "util/u_dynarray.h"

static const struct spirv_to_nir_options vulkan_opts = {
   .environment = NIR_SPIRV_VULKAN,
   .shared_addr_format = nir_address_format_32bit_offset,
   .global_addr_format = nir_address_format_64bit_global_32bit_offset,
   .temp_addr_format = nir_address_format_32bit_offset,
   .constant_addr_format = nir_address_format_64bit_global,
};

int
main(int argc, char **argv)
{
   if (argc != 4) {
      fprintf(stderr, "usage: %s <arch> <in.spv> <out.bin>\n", argv[0]);
      return 2;
   }

   const unsigned arch = (unsigned)strtoul(argv[1], NULL, 0);

   int fd = open(argv[2], O_RDONLY);
   if (fd < 0) {
      perror("open");
      return 1;
   }

   struct stat st;
   if (fstat(fd, &st) != 0) {
      perror("fstat");
      return 1;
   }

   void *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
   close(fd);
   if (map == MAP_FAILED) {
      perror("mmap");
      return 1;
   }

   glsl_type_singleton_init_or_ref();

   const nir_shader_compiler_options *opts =
      pan_get_nir_shader_compiler_options(arch, MESA_SHADER_COMPUTE, false);
   if (opts == NULL) {
      fprintf(stderr, "no compiler options for arch %u\n", arch);
      return 1;
   }

   nir_shader *nir = spirv_to_nir((const uint32_t *)map,
                                  (size_t)st.st_size / 4, NULL,
                                  MESA_SHADER_COMPUTE, "main",
                                  &vulkan_opts, opts);
   if (nir == NULL) {
      fprintf(stderr, "spirv_to_nir() found no compute entry point 'main'\n");
      return 1;
   }

   fprintf(stderr, "nir: wg %u/%u/%u\n", nir->info.workgroup_size[0],
           nir->info.workgroup_size[1], nir->info.workgroup_size[2]);

   pan_preprocess_nir(nir, arch);

   struct pan_compile_inputs inputs = {
      .arch = arch,
      .model = NULL,      /* pan_compile.c does the same when only arch is known */
   };
   struct pan_shader_info info = {0};
   pan_postprocess_nir(nir, &inputs, &info);

   struct util_dynarray binary;
   util_dynarray_init(&binary, NULL);
   pan_shader_compile(nir, &inputs, &binary, &info);

   if (binary.size == 0) {
      fprintf(stderr, "backend produced no machine code\n");
      return 1;
   }

   printf("ISA bytes=%u work_regs=%u tls=%u wls=%u\n", binary.size,
          info.work_reg_count, info.tls_size, info.wls_size);

   FILE *out = fopen(argv[3], "wb");
   if (out == NULL) {
      perror("fopen");
      return 1;
   }
   if (fwrite(binary.data, 1, binary.size, out) != binary.size) {
      perror("fwrite");
      fclose(out);
      return 1;
   }
   fclose(out);
   return 0;
}
