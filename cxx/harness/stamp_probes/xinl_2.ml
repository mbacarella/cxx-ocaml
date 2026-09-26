module F (X : sig type t end) = struct type u = U of X.t let e x = U
  x end module A = struct type t = int end module B = struct module M0 =
  struct end include F(A) end let z = B.e
