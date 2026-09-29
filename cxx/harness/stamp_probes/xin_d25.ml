module type S = sig type t = private string val of_string : string -> t end
module String_id : sig include S end = struct type t = string
let of_string s = s end
let x = (String_id.of_string "foo" :> string)
