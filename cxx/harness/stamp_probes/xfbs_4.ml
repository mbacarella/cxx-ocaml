module F (X: sig type t end) = struct module N = struct type w = int module
  M = struct type v = X.t let q : v * w = assert false end end end module A =
  struct type t = bool end module C = F(A) let q = C.N.M.q
