module type S = sig type t = string end
module String_id : sig include S end = struct type t = string end
