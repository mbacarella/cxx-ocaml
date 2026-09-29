module type S = sig type u val e : u end module F (X: sig type t end) :
  S with type u = X.t list = struct type u = X.t list let e = [] end module
  A = struct type t end module C = F(A) let e = C.e
