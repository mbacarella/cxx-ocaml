module PC = struct exception E of int end
let y x = PC.E x
type t = E | F
