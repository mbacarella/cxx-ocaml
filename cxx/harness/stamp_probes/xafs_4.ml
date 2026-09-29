module F (X: sig type t end) : sig module N : sig val z : int end end =
  struct module N = struct let z = 0 end end module A = struct type t =
  bool end module M = struct module C = F(A) let z = C.N.z end let z2 = M.C.N.z
