(* an exception in a nested module, then a rebinding of it *)
module M = struct module N = struct exception A end end
exception B = M.N.A
exception C
