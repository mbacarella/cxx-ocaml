module type S = sig type t val x : int val y : int end
module String_id : sig include S end = struct type t = string let x = 1
let y = 1 end
