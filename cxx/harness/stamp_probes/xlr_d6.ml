module rec Strengthen2 : sig type t val f : t -> t module M : sig type u end
  module R : sig type v end end = struct type t = A | B let _ = (A :
  Strengthen2.t) let f x = if true then A else Strengthen2.f B module M = struct
  type u = C end module rec R : sig type v = Strengthen2.R.v end = struct type v
  = D let _ = (D : R.v) let _ = (D : Strengthen2.R.v) end end
