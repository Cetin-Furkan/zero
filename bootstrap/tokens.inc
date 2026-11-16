; =============================================================================
; Zero Stage-0 Bootstrap — Token, Keyword & Type Constants (`bootstrap/tokens.inc`)
; =============================================================================
; Every Token in `space_tok` is an exact 16-byte cacheline-friendly record:
;   +0  u8   kind       (TOK_*)
;   +1  u8   subkind    (KW_* when kind == TOK_KW, else flags)
;   +2  u16  len        (source byte length, or decoded byte len for TOK_STR)
;   +4  u32  line       (1-based source line number)
;   +8  u64  payload    (sym_id | int_val | rodata_offset)
; =============================================================================

%define TOK_SIZE        16

; --- Core Token Kinds ---
%define TOK_EOF         0
%define TOK_IDENT       1
%define TOK_KW          2
%define TOK_INT         3
%define TOK_STR         4
%define TOK_CHAR        5

; --- Single-Character Sigils & Delimiters ---
%define TOK_LPAREN      10      ; (
%define TOK_RPAREN      11      ; )
%define TOK_LBRACE      12      ; {
%define TOK_RBRACE      13      ; }
%define TOK_LBRACK      14      ; [
%define TOK_RBRACK      15      ; ]
%define TOK_LT          16      ; <
%define TOK_GT          17      ; >
%define TOK_AT          18      ; @
%define TOK_PIPE        19      ; |
%define TOK_COLON       20      ; :
%define TOK_COMMA       21      ; ,
%define TOK_DOT         22      ; .
%define TOK_ASSIGN      23      ; =
%define TOK_PLUS        24      ; +
%define TOK_MINUS       25      ; -
%define TOK_STAR        26      ; *
%define TOK_SLASH       27      ; /
%define TOK_PERCENT     28      ; %
%define TOK_AMP         29      ; &
%define TOK_CARET       30      ; ^
%define TOK_TILDE       31      ; ~
%define TOK_BANG        32      ; !
%define TOK_QUEST       33      ; ?

; --- Multi-Character Zero Operators ---
%define TOK_COLON2      40      ; ::  (invariant / type / bit-pack def)
%define TOK_WALRUS      41      ; :=  (mutable runtime variable binding)
%define TOK_DOT2        42      ; ..  (range 0..N)
%define TOK_ARROW       43      ; ->  (flow / return / system edge)
%define TOK_EQ          44      ; ==
%define TOK_NE          45      ; !=
%define TOK_LE          46      ; <=
%define TOK_GE          47      ; >=
%define TOK_SHL         48      ; <<
%define TOK_SHR         49      ; >>
%define TOK_PLUS_EQ     50      ; +=
%define TOK_MINUS_EQ    51      ; -=
%define TOK_STAR_EQ     52      ; *=
%define TOK_SLASH_EQ    53      ; /=
%define TOK_PIPE_EQ     54      ; |=
%define TOK_AMP_EQ      55      ; &=
%define TOK_CARET_EQ    56      ; ^=
%define TOK_AND2        57      ; &&
%define TOK_OR2         58      ; ||
%define TOK_PERCENT_EQ  59      ; %=
%define TOK_SHL_EQ      60      ; <<=
%define TOK_SHR_EQ      61      ; >>=

; --- Pre-Interned Canonical Keyword IDs (`sym_id` 1 .. KW_COUNT) ---
%define KW_FN           1
%define KW_RETURN       2
%define KW_IF           3
%define KW_ELSE         4
%define KW_FOR          5
%define KW_IN           6
%define KW_LOOP         7
%define KW_BREAK        8
%define KW_CONTINUE     9
%define KW_RECOVER      10
%define KW_PRINT        11
%define KW_SYSCALL      12
%define KW_TRUE         13
%define KW_FALSE        14
%define KW_BIT          15
%define KW_BOOL         16
%define KW_U8           17
%define KW_U16          18
%define KW_U32          19
%define KW_U64          20
%define KW_I8           21
%define KW_I16          22
%define KW_I32          23
%define KW_I64          24
%define KW_F32          25
%define KW_F64          26
%define KW_STR          27
%define KW_BYTES        28
%define KW_MASK64       29
%define KW_RING         30
%define KW_SLAB         31
%define KW_SHARED       32
%define KW_ARENA        33
%define KW_MMIO         34
%define KW_REL          35
%define KW_CELL64       36
%define KW_COLD         37
%define KW_SYSTEM       38
%define KW_STAGE        39
%define KW_FAIL         40
%define KW_COUNT        40

; --- Compiler Type Representation Tags ---
%define TY_I64          0       ; 64-bit signed integer
%define TY_U8           1
%define TY_I8           2
%define TY_U16          3
%define TY_I16          4
%define TY_U32          5
%define TY_I32          6
%define TY_U64          7       ; 64-bit unsigned integer (`u64`, `mask64`, `bool`, `bit`)
%define TY_INT_LIT      8       ; non-negated integer literal (signed default; coerces with u*)
%define TY_BITPACK_BASE 0x100   ; 0x100 + type_idx -> bit-partitioned `<...|...>` (unsigned)
%define TY_STRUCT_BASE  0x200   ; 0x200 + type_idx -> record struct `{ ... }`

%define TY_BASE_MASK     0x0FFF
%define TY_FLAG_VIEW_ONE 0x1000 ; 8-byte single-instance typed overlay pointer (`@Type`, `@view<T>(addr)`)
%define TY_FLAG_SPAN     0x2000 ; 16-byte `{ base: u64, len: u64 }` bounded span (`@view<T>(addr, len)`)
%define TY_FLAG_MMIO     0x4000 ; marks `@mmio<T>` volatile aperture

; --- User Type Kind Constants ---
%define UKIND_BITPACK   1
%define UKIND_STRUCT    2

; --- Topological Space Kind Constants ---
%define SKIND_SLAB      1
%define SKIND_ARENA     2
%define SKIND_PORT      3

; --- Target Virtual Memory Constants ---
%define TARGET_BASE     0x00400000
%define TARGET_HDR_LEN  176             ; 64B Ehdr + 2 x 56B Phdr
%define DATA_VADDR_BASE 0x10000000      ; Disjoint RW `.bss` segment base
%define FAULT_WORD_VADDR 0x10000000     ; qword [0x10000000] = live scalar fault code

; --- Symbol Intern Space Offsets (`space_sym` = 64 MiB) ---
%define SYM_HASH_MASK       0xFFFF
%define SYM_META_OFF        0x100000

; --- `space_ast` Partition Map (64 MiB Arena) ---
%define AST_CONST_OFF       0x000000
%define AST_TYPE_OFF        0x020000
%define AST_FIELD_OFF       0x030000
%define AST_SPACE_OFF       0x040000
%define AST_FN_OFF          0x050000
%define AST_CALLFIX_OFF     0x060000
%define AST_LOCAL_OFF       0x0A0000
%define AST_BRK_PATCH_OFF   0x0B0000
%define AST_BRK_CNT_OFF     0x0B2000
%define AST_CNT_PATCH_OFF   0x0B4000
%define AST_CNT_CNT_OFF     0x0B6000
%define AST_REC_PATCH_OFF   0x0B8000
%define AST_REC_CNT_OFF     0x0BA000
%define AST_INTERP_BUF_OFF  0x0C0000
