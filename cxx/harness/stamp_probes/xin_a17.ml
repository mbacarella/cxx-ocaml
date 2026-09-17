module type S = sig type t end
let () =
let module String_id : sig include S end = struct type t = string end in ()
