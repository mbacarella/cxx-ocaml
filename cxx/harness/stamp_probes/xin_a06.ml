module type S = sig type t end
module String_id : sig include S val x : int end = struct type t = string
let x = 1 end
