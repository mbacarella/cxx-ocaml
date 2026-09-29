let f x = Some x let y = match f 1 with Some s -> s ^ "a" | None -> ""
