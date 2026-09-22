let a = 1
open struct
  type t = C
  let k = 2
end
let b = k
module G () = struct type t = int let v = 0 end
module H = G ()
