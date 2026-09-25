module PC = struct
  type _ op = Y : int -> unit op  type _ eff += E : 'a op -> 'a eff end
open PC
let y x = Effect.perform (E (Y x))
type t = E | F
