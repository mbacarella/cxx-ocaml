module PC = struct type o = E of int  exception E of string end
let y x = PC.E x
