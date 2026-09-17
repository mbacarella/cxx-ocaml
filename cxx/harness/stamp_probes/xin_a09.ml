module type S = sig type t end
module String_id : S = struct type t = string end
