module type S = sig type t end
module String_id : sig include S end = struct type t = string end
module String_id2 : sig include S end = struct type t = string end
