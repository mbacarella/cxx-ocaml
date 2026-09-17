module type S = sig type t = private string end
module String_id : sig include S end = struct type t = string end
type u = String_id.t
