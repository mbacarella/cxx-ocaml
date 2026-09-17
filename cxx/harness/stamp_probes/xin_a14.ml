module type S = sig type t end
module String_id : sig include S include S end = struct type t = string end
