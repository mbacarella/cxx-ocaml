module M = struct
  type t = { p : int }
  let k = 2
end
include M
let b = k
