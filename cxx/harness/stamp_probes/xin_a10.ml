module type S = sig type t end
module String_id : sig include sig type t end end = struct type t = string end
