module F (X: sig type t end) : sig val e : int val f : X.t -> X.t end =
  struct let e = 0 let f x = x end module A = struct type t = int end module
  C = F(A) let e = C.e let f = C.f
