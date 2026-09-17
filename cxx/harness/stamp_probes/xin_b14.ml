module A = struct module Make (M : sig val x : int end) : sig type 'a t
val x : int t end = struct type 'a t = int let x = 1 end end
