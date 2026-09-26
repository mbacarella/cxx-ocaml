module type S = sig val e : int end module F (X: sig type t end) : S =
  struct let e = 0 end module A = struct type t end module C = F(A) let e = C.e
