-- The event both sides use. Named rather than inlined so a typo is a load-time
-- problem in one place instead of a silent no-op on one side.
DifficultyGame = DifficultyGame or {}
DifficultyGame.EVENT_APPLY = 'difficulty:apply'
DifficultyGame.EVENT_REQUEST = 'difficulty:request'
DifficultyGame.DEFAULT = 'normal'
