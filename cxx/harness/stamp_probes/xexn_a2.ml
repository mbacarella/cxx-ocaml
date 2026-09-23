(* an exception inside a module cites the same path object *)
module M = struct exception A end
exception B
