module type S = sig type t end
module String_id : sig include S end = struct type t = string end
let f (x : String_id.t) = x
