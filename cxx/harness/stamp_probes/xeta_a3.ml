let f1 ?(x = 1) (y : int) = ignore x; ignore y
let r = ref f1
let h = List.iter r.contents [1]
