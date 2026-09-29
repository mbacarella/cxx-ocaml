module F (X : sig type t end) = struct type u = X.t list let e : u =
  [] end module A = struct type t = int end include F(A) let y = e
