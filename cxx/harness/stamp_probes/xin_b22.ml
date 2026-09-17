module type S = sig type t = private string end
module A = struct
module Make (M : sig val x : int end) : S with type t = string = struct
type t = string end end
