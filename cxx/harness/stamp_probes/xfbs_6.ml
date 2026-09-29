module F (X: sig type t end) = struct type w = int module N = struct type
  w = string let z : w = "" end let q : w = 0 end module A = struct type t =
  bool end module C = F(A) let z = C.N.z let q = C.q
