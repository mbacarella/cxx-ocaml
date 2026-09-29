module Make (M : sig end) = struct let f s = s end
include Make (struct end)
let x = f 1
