(* A plain module with the same ascription is not a functor. *)
module M : sig type k type 'a t val a : 'a t end =
struct type k = int type 'a t = 'a list let a = [] end
