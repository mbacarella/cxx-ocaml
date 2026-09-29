module type S = sig type 'a t val x : int t end
module A = struct module Make (M : sig val x : int end) : S = struct
type 'a t = int let x = 1 end end
