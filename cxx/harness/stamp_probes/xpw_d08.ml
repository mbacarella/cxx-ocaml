module type S = sig type t module M : sig type s end module M2 : sig type s2
  end end
type u = (module S with type t = int)
