module type S = sig module N : sig type t end end
module String_id : sig include S end = struct
module N = struct type t = string end end
