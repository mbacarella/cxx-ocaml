module A = struct module Make (M : sig val x : int end) : sig
type 'a t = private 'a list end = struct type 'a t = 'a list end end
