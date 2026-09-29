module type S = sig module M : sig type s end type t end
type u = (module S with type t = int)
