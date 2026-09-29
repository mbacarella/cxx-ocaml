module F (X: sig type t end) : sig type u module N : sig val z : u val
  y : X.t module M : sig val q : u * X.t end end end = struct type u = int
  module N = struct let z = 0 let y = assert false module M = struct let q =
  assert false end end end module A = struct type t = bool end module C =
  F(A) let z = C.N.z let y = C.N.y let q = C.N.M.q
