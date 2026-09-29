module M = struct module N = struct type t = A | B end end
include M.N
