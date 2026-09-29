module F (X: sig type t end) : sig module N : sig val z : X.t list end end
  = struct module N = struct let z = [] end end module A = struct type t =
  bool end module C = F(A) let z = C.N.z
