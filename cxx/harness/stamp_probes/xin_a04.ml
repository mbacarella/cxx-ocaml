module type S = sig val x : int end
module String_id : sig include S end = struct let x = 1 end
