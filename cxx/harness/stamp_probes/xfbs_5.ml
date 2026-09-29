module F (X: sig type t end) = struct module N = struct type w = int module
  M = struct type w = string let q : w = "" end let r : w = 0 end end module
  A = struct type t = bool end module C = F(A) let q = C.N.M.q let r = C.N.r
