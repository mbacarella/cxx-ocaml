(* an exception with a payload still cites the one path *)
exception A of int
exception B of string * int
