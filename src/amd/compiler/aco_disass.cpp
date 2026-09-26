#include "llvm/ac_llvm_util.h"

#include "llvm-c/Disassembler.h"
#include <llvm/ADT/StringRef.h>
#include <llvm/MC/MCDisassembler/MCDisassembler.h>

bool
print_asm_llvm(Program* program, std::vector<uint32_t>& binary, unsigned exec_size, FILE* output)
{
   std::vector<bool> referenced_blocks = get_referenced_blocks(program);

   std::vector<llvm::SymbolInfoTy> symbols;
   std::vector<std::array<char, 16>> block_names;
   block_names.reserve(program->blocks.size());
   for (Block& block : program->blocks) {
      if (!referenced_blocks[block.index])
         continue;
      std::array<char, 16> name;
      sprintf(name.data(), "BB%u", block.index);
      block_names.push_back(name);
      symbols.emplace_back(block.offset * 4,
                           llvm::StringRef(block_names[block_names.size() - 1].data()), 0);
   }

   const char* features = "";
   if (program->gfx_level >= GFX10 && program->wave_size == 64) {
      features = "+wavefrontsize64";
   }

   LLVMDisasmContextRef disasm =
      LLVMCreateDisasmCPUFeatures("amdgcn-mesa-mesa3d", ac_get_llvm_processor_name(program->family),
                                  features, &symbols, 0, NULL, NULL);

   size_t pos = 0;
   bool invalid = false;
   unsigned next_block = 0;

   unsigned prev_size = 0;
   unsigned prev_pos = 0;
   unsigned repeat_count = 0;
   while (pos <= exec_size) {
      bool new_block =
         next_block < program->blocks.size() && pos == program->blocks[next_block].offset;
      if (pos + prev_size <= exec_size && prev_pos != pos && !new_block &&
          memcmp(&binary[prev_pos], &binary[pos], prev_size * 4) == 0) {
         repeat_count++;
         pos += prev_size;
         continue;
      } else {
         if (repeat_count)
            fprintf(output, "\t(then repeated %u times)\n", repeat_count);
         repeat_count = 0;
      }

      print_block_markers(output, program, referenced_blocks, &next_block, pos);

      /* For empty last block, only print block marker. */
      if (pos == exec_size)
         break;

      char outline[1024];
      std::pair<bool, size_t> res = disasm_instr(program->gfx_level, disasm, binary.data(),
                                                 exec_size, pos, outline, sizeof(outline));
      invalid |= res.first;

      print_instr(output, binary, outline, res.second, pos);

      prev_size = res.second;
      prev_pos = pos;
      pos += res.second;
   }
   assert(next_block == program->blocks.size());

   LLVMDisasmDispose(disasm);

   print_constant_data(output, program);

   return invalid;
}

int
main(int argc, char** argv)
{
   fprintf(stderr, "This is a disassembler for AMDGPU machine code. It is not a general-purpose "
                   "disassembler and may not work correctly on all inputs.\n");
   fprintf(stderr, "Usage: %s <input file>\n", argv[0]);
   if (argc != 2)
      return 1;

   FILE* input = fopen(argv[1], "r");
   if (!input) {
      fprintf(stderr, "Failed to open input file '%s'\n", argv[1]);
      return 1;
   }

   Program program;
   std::vector<uint32_t> binary;
   if (!read_program(argv[1], &program, &binary))
      return 1;

   FILE* output = fopen(argv[2], "w");
   if (!output) {
      fprintf(stderr, "Failed to open output file '%s'\n", argv[2]);
      return 1;
   }

   bool invalid = print_asm_llvm(&program, binary, program.exec_size, output);
   fclose(output);

   return invalid ? 1 : 0;
}
