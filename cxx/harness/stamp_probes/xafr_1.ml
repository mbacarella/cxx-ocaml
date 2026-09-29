module F (X: sig type t end) : sig type u val e : u end = struct type u =
  int let e = 0 end module A = struct type t end module C = F(A) let e = C.e
