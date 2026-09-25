module PC = struct exception E of int end
let f = function PC.E n -> n | _ -> 0
type t = E | F
