let p = Lexing.dummy_pos
let l = p.Lexing.pos_lnum
let q = { p with Lexing.pos_cnum = 3 }
let r = { Lexing.pos_fname = ""; pos_lnum = 1; pos_bol = 0; pos_cnum = 0 }
