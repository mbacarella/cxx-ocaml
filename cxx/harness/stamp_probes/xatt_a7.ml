(* a `with type` clause takes the clause's attributes *)
module type S = sig type t [@@immediate] end
module type T = S with type t = int
