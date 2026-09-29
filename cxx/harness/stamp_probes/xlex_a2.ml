module PC = struct type o = Y of int  exception E of o end
open PC
let y x = E (Y x)
type t = E | F
